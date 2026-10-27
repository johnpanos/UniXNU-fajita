// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita thermal policy (package/FajitaThermal.cpp): TSENS decoding and the CPU cluster step governor, with no kernel dependency so the
// host test (package/tests/fajita-thermal-test.cpp, scripts/check-fajita-thermal-host.py) runs it.
//
// TSENS v2 (Linux drivers/thermal/qcom/tsens-v2.c, sdm845.dtsi tsens0 0x0c263000 13 sensors, tsens1 0x0c265000 8 sensors):
// TM_Sn_STATUS at 0xa0 + 4n, bit 21 VALID, bits 11:0 LAST_TEMP in signed deci-degC.
// Zones (sdm845.dtsi thermal-zones): tsens0 1-4 cpu0-3 (silver), 5 L3 cluster0, 6 L3 cluster1, 7-10 cpu4-7 (gold), 11/12 gpu,
// 0 aoss0; tsens1 0 aoss1, 1 q6-modem, 2 mem, 3 wlan, 4 q6-hvx, 5 camera, 6 video, 7 modem. CPU trips: passive 90 and 95 C,
// critical 110 C (the hardware's own reset is above that).
//
// The governor runs once a second per cluster, on the hottest of the cluster's sensors (and its L3's), and moves the cluster's
// highest allowed OSM LUT index:
//   t >= crit (105 C)          straight to the floor
//   t >= hot  (95 C)           two levels down
//   t >= trip (90 C)           one level down
//   t <= release (85 C)        one level up (towards the top) after `hold` quiet seconds since the last step down
//   no valid sensor            nothing moves; `invalid` counts it
// It never goes below `floor` or above `top`.
#ifndef FAJITA_THERMAL_POLICY_H
#define FAJITA_THERMAL_POLICY_H
#include <stdint.h>

#define FTP_TSENS0_COUNT 13u
#define FTP_TSENS1_COUNT 8u

static inline int ftp_decode(uint32_t status, int32_t *deci) {
    if (!(status & (1u << 21))) return 0;
    *deci = (int32_t)(status << 20) >> 20;
    return 1;
}

typedef struct {
    int32_t trip, hot, crit, release;   // deci-degC
    unsigned hold;                      // seconds below release before a step up
} ftp_trips;

static inline ftp_trips ftp_default_trips(void) {
    ftp_trips t = {900, 950, 1050, 850, 5};
    return t;
}

typedef struct {
    int level;           // highest allowed LUT index now
    int floor, top;      // its bounds
    unsigned quiet;      // seconds at or below release since the last step down
    uint32_t downs, ups, invalid;
    int32_t last;        // last hottest reading (deci-degC), -1000 before any
} ftp_cluster;

static inline void ftp_init(ftp_cluster *c, int floor, int top) {
    c->floor = floor < 0 ? 0 : floor;
    c->top = top < c->floor ? c->floor : top;
    c->level = c->top;
    c->quiet = 0;
    c->downs = c->ups = c->invalid = 0;
    c->last = -1000;
}

// hottest: the cluster's hottest valid reading, or valid = 0. Returns 1 when the level changed.
static inline int ftp_step(ftp_cluster *c, const ftp_trips *t, int32_t hottest, int valid) {
    if (!valid) { c->invalid++; return 0; }
    c->last = hottest;
    int to = c->level;
    if (hottest >= t->crit) to = c->floor;
    else if (hottest >= t->hot) to = c->level - 2;
    else if (hottest >= t->trip) to = c->level - 1;
    if (to < c->floor) to = c->floor;
    if (to < c->level) { c->level = to; c->quiet = 0; c->downs++; return 1; }
    if (hottest <= t->release) {
        if (c->level < c->top && ++c->quiet >= t->hold) { c->level++; c->quiet = 0; c->ups++; return 1; }
    } else {
        c->quiet = 0;
    }
    return 0;
}

// The sensors of each cluster: indices into the combined table (tsens0 n -> n, tsens1 n -> 16 + n).
static const unsigned ftp_silver[] = {1, 2, 3, 4, 5};
static const unsigned ftp_gold[] = {7, 8, 9, 10, 6};

// hottest valid of `ids` in `deci`/`ok` (indexed 0..31); returns 1 when any was valid.
static inline int ftp_hottest(const int32_t *deci, const uint8_t *ok, const unsigned *ids, unsigned n, int32_t *out) {
    int any = 0;
    int32_t best = -1000;
    for (unsigned i = 0; i < n; i++) {
        if (!ok[ids[i]]) continue;
        if (!any || deci[ids[i]] > best) best = deci[ids[i]];
        any = 1;
    }
    *out = best;
    return any;
}

#endif
