// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita thermal: the SDM845 TSENS sensors (21 of them on two controllers) and a CPU thermal governor on top of the OSM votes of
// FajitaCpuFreq.cpp. The policy (decoding, trips, steps) is FajitaThermalPolicy.h, host-tested (scripts/check-fajita-thermal-host.py).
//
// fajita-thermal=<n> (default 0: nothing is mapped or read):
//   1  read only: every sensor once a second, a status row (fajita_thermal_row), `kcmd thermal`, a log line when a cluster's hottest
//      sensor crosses 10 C steps above 70 C
//   2  as 1, and the governor: each CPU cluster's highest allowed OSM LUT index follows its hottest sensor (cpu cores + its L3):
//      down one level at 90 C, two at 95 C, to the floor at 105 C, back up one level after 5 s at or below 85 C (Linux's sdm845 cpuN
//      passive trips are 90/95 C, critical 110 C). Needs fajita-cpufreq=2 (the board default), which votes the top level; without the
//      governor the CPUs sit at that level whatever the temperature, and only the OSM's own limits hardware stands between them and
//      the critical trip.
// fajita-thermal-floor-silver / -floor-gold=<kHz> (defaults 748800 / 825600): the lowest level the governor uses, the first LUT row at
// or above it.
// kcmd thermal [sim silver|gold DECI_C SECONDS | sim off]: the sensors and the governor; `sim` replaces a cluster's hottest reading
// for a while (an unattended test of the steps without heating the phone).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaThermalPolicy.h"

extern "C" {
char fajita_thermal_row[256];
volatile int32_t fajita_thermal_deci[32];   // last reading per sensor (tsens0 n -> n, tsens1 n -> 16 + n), deci-degC
volatile uint32_t fajita_thermal_valid;     // bit per sensor
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
int fajita_cpufreq_lut(unsigned dom, unsigned *rows, const uint32_t **khz) __attribute__((weak));
uint32_t fajita_cpufreq_thermal_limit(unsigned dom, int max) __attribute__((weak));
extern char native_cpufreq_row[120] __attribute__((weak));        // FajitaCpuFreq.cpp: the OSM state (cur = the applied vote)
extern char fajita_cpufreq_rows[6][256] __attribute__((weak));    // [2]: the last per-core clock measurement (every 30 s)
}

namespace {
const char *const kNames[32] = {
    "aoss0", "cpu0", "cpu1", "cpu2", "cpu3", "l3-0", "l3-1", "cpu4", "cpu5", "cpu6", "cpu7", "gpu-top", "gpu-bot", nullptr, nullptr, nullptr,
    "aoss1", "q6-modem", "mem", "wlan", "q6-hvx", "camera", "video", "modem"};
const uint64_t kBase[2] = {0x0c263000, 0x0c265000};
const unsigned kCount[2] = {FTP_TSENS0_COUNT, FTP_TSENS1_COUNT};
OSSharedPtr<IOMemoryMap> gMap[2];
volatile uint32_t *gReg[2];
unsigned gMode;
ftp_trips gTrips;

struct Cluster {
    const char *name;
    const unsigned *ids;
    unsigned nids;
    uint32_t floorKHz;
    bool ready;
    ftp_cluster c;
    const uint32_t *khz;
    unsigned rows;
    uint32_t votedKHz;
    int32_t logged;          // last 10 C band logged
    volatile int32_t simDeci;
    volatile uint32_t simLeft; // seconds of simulation left
};
Cluster gCl[2] = {
    {"silver", ftp_silver, 5, 748800, false, {}, nullptr, 0, 0, 0, 0, 0},
    {"gold", ftp_gold, 5, 825600, false, {}, nullptr, 0, 0, 0, 0, 0},
};
uint32_t gTicks;

bool mapAll() {
    for (unsigned b = 0; b < 2; ++b) {
        if (gReg[b]) continue;
        IODeviceMemory *m = IODeviceMemory::withRange(kBase[b], 0x1000);
        if (m) {gMap[b] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain); m->release();}
        if (!gMap[b]) return false;
        gReg[b] = reinterpret_cast<volatile uint32_t *>(gMap[b]->getVirtualAddress());
    }
    return true;
}

void sample(int32_t deci[32], uint8_t ok[32]) {
    uint32_t valid = 0;
    for (unsigned i = 0; i < 32; ++i) {deci[i] = -1000; ok[i] = 0;}
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned n = 0; n < kCount[b]; ++n) {
            const unsigned id = b * 16 + n;
            int32_t t;
            if (ftp_decode(gReg[b][(0xa0 + 4 * n) / 4], &t)) {deci[id] = t; ok[id] = 1; valid |= 1u << id; fajita_thermal_deci[id] = t;}
        }
    fajita_thermal_valid = valid;
}

int floorIndex(const Cluster &cl) {
    for (unsigned i = 0; i < cl.rows; ++i) if (cl.khz[i] >= cl.floorKHz) return int(i);
    return 0;
}

void row(const int32_t deci[32], const uint8_t ok[32]) {
    int32_t hot = -1000; unsigned hotId = 0;
    for (unsigned i = 0; i < 32; ++i) if (ok[i] && deci[i] > hot) {hot = deci[i]; hotId = i;}
    int n = snprintf(fajita_thermal_row, sizeof fajita_thermal_row, "[thermal] m%u t=%u max %d.%d C (%s)", gMode, gTicks, hot / 10,
                     (hot < 0 ? -hot : hot) % 10, kNames[hotId] ? kNames[hotId] : "?");
    for (const Cluster &cl : gCl) {
        if (n <= 0 || n >= int(sizeof fajita_thermal_row)) break;
        if (!cl.ready) {n += snprintf(fajita_thermal_row + n, sizeof fajita_thermal_row - n, " %s:-", cl.name); continue;}
        n += snprintf(fajita_thermal_row + n, sizeof fajita_thermal_row - n, " %s:%d.%dC L%d/%d %uMHz d%u u%u%s", cl.name, cl.c.last / 10,
                      (cl.c.last < 0 ? -cl.c.last : cl.c.last) % 10, cl.c.level, cl.c.top, cl.votedKHz / 1000, cl.c.downs, cl.c.ups,
                      cl.simLeft ? " SIM" : "");
    }
}

void tick() {
    int32_t deci[32]; uint8_t ok[32];
    sample(deci, ok);
    ++gTicks;
    for (unsigned k = 0; k < 2; ++k) {
        Cluster &cl = gCl[k];
        int32_t hot; int valid = ftp_hottest(deci, ok, cl.ids, cl.nids, &hot);
        if (cl.simLeft) {hot = cl.simDeci; valid = 1; cl.simLeft = cl.simLeft - 1;}
        if (gMode >= 2 && !cl.ready && fajita_cpufreq_lut && fajita_cpufreq_thermal_limit) {
            const int top = fajita_cpufreq_lut(k, &cl.rows, &cl.khz);
            if (top >= 0) {
                ftp_init(&cl.c, floorIndex(cl), top);
                cl.votedKHz = cl.khz[top];
                cl.ready = true;
                IOLog("fajita-thermal: %s governor on: L%d..L%d (%u..%u MHz)\n", cl.name, cl.c.floor, cl.c.top, cl.khz[cl.c.floor] / 1000,
                      cl.khz[top] / 1000);
            }
        }
        if (cl.ready) {
            if (ftp_step(&cl.c, &gTrips, hot, valid)) {
                cl.votedKHz = fajita_cpufreq_thermal_limit(k, cl.c.level);
                IOLog("fajita-thermal: %s %d.%d C -> L%d %u MHz%s\n", cl.name, hot / 10, (hot < 0 ? -hot : hot) % 10, cl.c.level,
                      cl.votedKHz / 1000, cl.simLeft ? " (sim)" : "");
            }
        } else if (valid) {
            cl.c.last = hot;
        }
        const int32_t band = valid && hot >= 700 ? hot / 100 : 0;
        if (band != cl.logged) {
            if (band > cl.logged) IOLog("fajita-thermal: %s at %d.%d C\n", cl.name, hot / 10, hot % 10);
            cl.logged = band;
        }
    }
    row(deci, ok);
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-thermal");
    for (;;) {tick(); IOSleep(1000);}
}

struct Out {
    char *p; size_t cap, n;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (n >= cap) return;
        va_list ap; va_start(ap, fmt);
        int r = vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (r > 0) n = n + size_t(r) < cap ? n + size_t(r) : cap;
    }
};

size_t kcmdThermal(const char *args, size_t len, char *out, size_t cap) {
    char a[48] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    Out o{out, cap, 0};
    if (!strncmp(a, "sim ", 4)) {
        const char *w = a + 4;
        if (!strncmp(w, "off", 3)) {gCl[0].simLeft = gCl[1].simLeft = 0; o.add("thermal: sim off\n"); return o.n;}
        const int k = !strncmp(w, "silver ", 7) ? 0 : !strncmp(w, "gold ", 5) ? 1 : -1;
        if (k < 0) {o.add("usage: thermal sim silver|gold DECI_C SECONDS | sim off\n"); return o.n;}
        const char *p = w + (k ? 5 : 7);
        long v[2] = {0, 0}; unsigned nv = 0;
        while (*p && nv < 2) {
            while (*p == ' ') ++p;
            if (*p < '0' || *p > '9') break;
            long x = 0; while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
            v[nv++] = x;
        }
        if (nv < 2 || v[1] > 600) {o.add("usage: thermal sim silver|gold DECI_C SECONDS (<= 600)\n"); return o.n;}
        gCl[k].simDeci = int32_t(v[0]);
        gCl[k].simLeft = uint32_t(v[1]);
        o.add("thermal: %s reads %ld.%ld C for %ld s\n", gCl[k].name, v[0] / 10, v[0] % 10, v[1]);
        return o.n;
    }
    o.add("%s\n", fajita_thermal_row);
    for (unsigned i = 0; i < 32; ++i) {
        if (!kNames[i]) continue;
        if (fajita_thermal_valid & (1u << i)) o.add(" %s=%d.%d", kNames[i], fajita_thermal_deci[i] / 10, (fajita_thermal_deci[i] < 0 ? -fajita_thermal_deci[i] : fajita_thermal_deci[i]) % 10);
        else o.add(" %s=?", kNames[i]);
    }
    o.add("\n");
    for (const Cluster &cl : gCl)
        if (cl.ready) o.add("%s: L%d (floor L%d %u MHz, top L%d %u MHz) voted %u MHz, %u down %u up %u invalid\n", cl.name, cl.c.level, cl.c.floor,
                            cl.khz[cl.c.floor] / 1000, cl.c.top, cl.khz[cl.c.top] / 1000, cl.votedKHz / 1000, cl.c.downs, cl.c.ups, cl.c.invalid);
    if (&native_cpufreq_row) o.add("osm: %s\n", native_cpufreq_row);
    if (&fajita_cpufreq_rows) o.add("%s\n", fajita_cpufreq_rows[2]);
    return o.n;
}
}  // namespace

// Called once a second by the boot progress monitor (FajitaCpuFreq.cpp's sheng_cpufreq_poll); the first call after IOKit is up starts it.
extern "C" void fajita_thermal_poll(void) {
    static bool done;
    if (done) return;
    if (!IOService::getPlatform()) return;
    done = true;
    PE_parse_boot_argn("fajita-thermal", &gMode, sizeof gMode);
    if (!gMode) return;
    PE_parse_boot_argn("fajita-thermal-floor-silver", &gCl[0].floorKHz, sizeof gCl[0].floorKHz);
    PE_parse_boot_argn("fajita-thermal-floor-gold", &gCl[1].floorKHz, sizeof gCl[1].floorKHz);
    gTrips = ftp_default_trips();
    if (!mapAll()) {IOLog("fajita-thermal: TSENS map failed\n"); return;}
    if (fajita_usbdbg_register_kcmd)
        fajita_usbdbg_register_kcmd("thermal", kcmdThermal, "TSENS sensors and the CPU thermal governor: thermal [sim silver|gold DECI_C S | sim off]");
    thread_t t = nullptr;
    if (kernel_thread_start(worker, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    IOLog("fajita-thermal: mode %u, floors %u/%u kHz\n", gMode, gCl[0].floorKHz, gCl[1].floorKHz);
}
