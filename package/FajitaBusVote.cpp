// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita memory bus bandwidth: the RPMh BCM votes for DDR (MC0) and the LLCC/memory NoC (SH0) that Linux's interconnect drivers
// send and nothing on fajita did. The firmware leaves them at MC0 y=0x94, SH0 y=0x4a (TCS read-back, session-232253): a kernel
// memcpy ran at 538-832 MB/s and the GPU-composited UI at ~3.5 fps; with the votes, 3.4 GB/s and 9 fps.
//
// `fajita-ddr=<peak MB/s>` (default 7216, Linux's A630 vote: msm a6xx_gmu_set_freq, icc_set_bw(path, 0, MBps_to_icc(7216)));
// 0 leaves the firmware's votes. `kcmd bus [PEAK [AVG]] | off | read` changes or reads them at run time (off = the keepalive
// minimum x = y = 1, what bcm-voter.c sends for an unvoted keepalive BCM).
//
// Encoding (drivers/interconnect/qcom/bcm-voter.c bcm_aggregate, tcs_cmd_gen; sdm845.c nodes): bandwidth in kB/s;
// MC0 for node ebi (buswidth 4, 4 channels), SH0 for qns_llcc (buswidth 16, 4 channels); with cmd-db's aux data {le32 unit,
// le16 width, u8 vcd}: x = DIV_ROUND_UP(DIV_ROUND_UP(avg * width, buswidth * channels) * 1000, unit),
// y = DIV_ROUND_UP(DIV_ROUND_UP(peak * width, buswidth) * 1000, unit), 14 bits each; command BCM_TCS_CMD(commit, valid, x, y).
// Transport (rpmh-rsc.c, as FajitaTouchProbe's LDO28 vote): apps RSC drv 2 at 0x179e0000, ACTIVE TCS 0/1 at +0xd00; only an idle,
// untriggered TCS is used (searched from TCS 1 down, so the touch probe, which searches from 0, rarely meets it), commands with a
// completion response, polled. Runs once from the boot progress monitor (sheng_cpufreq_poll) as soon as IOKit is up.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
char fajita_bus_row[256];
volatile uint32_t fajita_bus_state[4];   // [0] votes sent, [1] last MC0 command, [2] last SH0 command, [3] last error (0 ok)
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
}

namespace {
enum : uint32_t {
    kCmdDbBase = 0x85fe0000, kCmdDbSize = 0x20000,
    kRscBase = 0x179e0000, kTcsOff = 0xd00, kTcsStride = 672, kCmdStride = 20, kTcsIrqClear = 0x08,
    kTcsWait = 0x10, kTcsControl = 0x14, kTcsStatus = 0x18, kTcsEnable = 0x1c, kCmdMsgid = 0x30, kCmdAddr = 0x34, kCmdData = 0x38,
    kCmdStatus = 0x3c, kCmdResp = 0x40, kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24,
    kMsgWriteResp = (1u << 16) | (1u << 8) | 8u, kMsgReadResp = (1u << 8) | 8u, kIssued = 1u << 8, kCompl = 1u << 16,
};
struct Bcm {const char *name; uint32_t buswidth, channels, addr, unit, width, vcd;};
Bcm gBcm[2] = {{"MC0", 4, 4, 0, 0, 0, 0}, {"SH0", 16, 4, 0, 0, 0, 0}};
bool gResolved;
OSSharedPtr<IOMemoryMap> gRsc;
IOLock *gLock;

struct Out {
    char *p; size_t cap, n;
    Out(char *b, size_t c) : p(b), cap(c), n(0) {if (c) p[0] = 0;}
    void add(const char *text) {add("%s", text);}
    template <typename A0, typename... A> void add(const char *fmt, A0 a0, A... a) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a0, a...);
        if (w > 0) n = n + size_t(w) >= cap ? cap - 1 : n + size_t(w);
    }
};

uint32_t le16(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8;}
uint32_t le32(const uint8_t *p) {return le16(p) | le16(p + 2) << 16;}
uint64_t divUp(uint64_t a, uint64_t b) {return b ? (a + b - 1) / b : 0;}

// cmd_db_read_addr + cmd_db_read_aux_data (drivers/soc/qcom/cmd-db.c layout) for MC0 and SH0, over a copy of the reservation.
bool resolve(Out &o) {
    if (gResolved) return true;
    IODeviceMemory *m = IODeviceMemory::withRange(kCmdDbBase, kCmdDbSize);
    if (!m) {o.add("cmd-db range"); return false;}
    OSSharedPtr<IOMemoryMap> map(m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
    m->release();
    if (!map) {o.add("cmd-db map"); return false;}
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return false;
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < kCmdDbSize / 4; ++i) {const uint32_t w = src[i]; memcpy(db + 4 * i, &w, 4);}
    enum {kHeader = 8, kRscHdr = 16, kSlaves = 8, kData = 8 + 16 * 8 + 8, kEntry = 24};
    unsigned found = 0;
    if (db[4] == 0xdb && db[5] == 0x30 && db[6] == 0x03 && db[7] == 0x0c) {
        for (unsigned i = 0; i < kSlaves; ++i) {
            const uint8_t *h = db + kHeader + kRscHdr * i;
            const unsigned slave = le16(h), header = le16(h + 2), data = le16(h + 4), count = le16(h + 6);
            if (!slave) break;
            for (unsigned j = 0; j < count; ++j) {
                const size_t at = size_t(kData) + header + size_t(j) * kEntry;
                if (at + kEntry > kCmdDbSize) break;
                const uint8_t *e = db + at;
                for (Bcm &b : gBcm) {
                    if (b.addr || strncmp(reinterpret_cast<const char *>(e), b.name, 8)) continue;
                    const size_t len = le16(e + 20), off = size_t(kData) + data + le16(e + 22);
                    if (len < 8 || off + len > kCmdDbSize) continue;
                    b.addr = le32(e + 16); b.unit = le32(db + off); b.width = le16(db + off + 4); b.vcd = db[off + 6];
                    ++found;
                }
            }
        }
    }
    IOFreeData(db, kCmdDbSize);
    if (found != 2 || !gBcm[0].unit || !gBcm[1].unit) {o.add("cmd-db: MC0/SH0 not found (%u)", found); return false;}
    gResolved = true;
    return true;
}

volatile uint32_t &rsc(uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(gRsc->getVirtualAddress() + off);}
uint32_t tcsReg(unsigned t, uint32_t r) {return kTcsOff + kTcsStride * t + r;}
uint32_t cmdReg(unsigned t, unsigned c, uint32_t r) {return tcsReg(t, r) + kCmdStride * c;}

// One AMC request of n commands {msgid, addr, data}: the TCS used, -1 none idle, -2 no completion within 100 ms.
int send(unsigned n, const uint32_t (*cmd)[3], uint32_t *status, uint32_t *resp, uint32_t *us) {
    int tcs = -1;
    for (int t = 1; t >= 0 && tcs < 0; --t)
        if (rsc(tcsReg(unsigned(t), kTcsStatus)) && !(rsc(tcsReg(unsigned(t), kTcsControl)) & kAmcTrigger)) tcs = t;
    if (tcs < 0) return -1;
    const unsigned t = unsigned(tcs);
    const uint32_t mask = (1u << n) - 1;
    rsc(tcsReg(t, kTcsEnable)) = 0;
    for (unsigned c = 0; c < n; ++c) {rsc(cmdReg(t, c, kCmdMsgid)) = cmd[c][0]; rsc(cmdReg(t, c, kCmdAddr)) = cmd[c][1]; rsc(cmdReg(t, c, kCmdData)) = cmd[c][2];}
    rsc(tcsReg(t, kTcsWait)) = mask;
    rsc(tcsReg(t, kTcsEnable)) = mask;
    const uint32_t control = rsc(tcsReg(t, kTcsControl)) & ~(kAmcTrigger | kAmcEnable);
    rsc(tcsReg(t, kTcsControl)) = control;
    rsc(tcsReg(t, kTcsControl)) = control | kAmcEnable;
    rsc(tcsReg(t, kTcsControl)) = control | kAmcEnable | kAmcTrigger;
    bool done = false;
    uint32_t waited = 0;
    for (; waited < 100000 && !done; waited += 10) {
        done = true;
        for (unsigned c = 0; c < n; ++c) {status[c] = rsc(cmdReg(t, c, kCmdStatus)); done &= (status[c] & (kIssued | kCompl)) == (kIssued | kCompl);}
        if (!done) IODelay(10);
    }
    for (unsigned c = 0; c < n && resp; ++c) resp[c] = rsc(cmdReg(t, c, kCmdResp));
    rsc(tcsReg(t, kTcsControl)) = control | kAmcEnable;
    rsc(tcsReg(t, kTcsControl)) = control;
    rsc(tcsReg(t, kTcsEnable)) = 0;
    rsc(kTcsOff + kTcsIrqClear) = 1u << t;
    if (us) *us = waited;
    return done ? tcs : -2;
}

// vote: peak/avg in MB/s; off sends the keepalive minimum; neither (read) only reads the BCMs back.
size_t run(bool write, bool off, uint64_t peakMBps, uint64_t avgMBps, char *buf, size_t cap) {
    Out o(buf, cap);
    if (!gLock) return 0;
    IOLockLock(gLock);
    int error = 0;
    do {
        if (!resolve(o)) {error = -2; break;}
        if (!gRsc) {
            IODeviceMemory *m = IODeviceMemory::withRange(kRscBase, 0x10000);
            if (m) {gRsc = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain); m->release();}
            if (!gRsc) {o.add("rsc map"); error = -12; break;}
        }
        uint32_t cmd[2][3], rd[2][3], st[2] = {}, resp[2] = {}, us = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const Bcm &b = gBcm[i];
            uint64_t x = divUp(divUp(avgMBps * 1000 * b.width, b.buswidth * b.channels) * 1000, b.unit);
            uint64_t y = divUp(divUp(peakMBps * 1000 * b.width, b.buswidth) * 1000, b.unit);
            if (off || (!x && !y)) x = y = 1;
            if (x > 0x3fff) x = 0x3fff;
            if (y > 0x3fff) y = 0x3fff;
            cmd[i][0] = kMsgWriteResp; cmd[i][1] = b.addr; cmd[i][2] = (1u << 30) | (1u << 29) | uint32_t(x) << 14 | uint32_t(y);
            rd[i][0] = kMsgReadResp; rd[i][1] = b.addr; rd[i][2] = 0;
            if (write) o.add("%s@%x u%u w%u vcd%u x%llu y%llu=%08x ", b.name, b.addr, b.unit, b.width, b.vcd, (unsigned long long)x,
                             (unsigned long long)y, cmd[i][2]);
        }
        if (write) {
            const int t = send(2, cmd, st, nullptr, &us);
            o.add("| vote %s tcs%d st%x/%x %uus ", off ? "off" : "on", t, st[0], st[1], us);
            if (t < 0) {error = t; break;}
            fajita_bus_state[0] = fajita_bus_state[0] + 1;
            fajita_bus_state[1] = cmd[0][2]; fajita_bus_state[2] = cmd[1][2];
        }
        const int r = send(2, rd, st, resp, &us);
        o.add("| read tcs%d MC0 %08x SH0 %08x", r, resp[0], resp[1]);
        // The BCMs echo the vote without the commit bit (30): anything else means the vote did not take.
        if (write && (r < 0 || ((resp[0] ^ cmd[0][2]) & 0x3fffffffu) || ((resp[1] ^ cmd[1][2]) & 0x3fffffffu))) {o.add(" MISMATCH"); error = -5;}
    } while (0);
    fajita_bus_state[3] = uint32_t(error);
    IOLockUnlock(gLock);
    return o.n;
}

size_t kcmdBus(const char *args, size_t len, char *out, size_t cap) {
    char a[32] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    if (!strncmp(a, "read", 4) || !a[0]) {size_t n = run(false, false, 0, 0, out, cap); if (n + 1 < cap) {out[n++] = '\n'; out[n] = 0;} return n;}
    const bool off = !strncmp(a, "off", 3);
    uint64_t peak = 7216, avg = 0;
    const char *p = a;
    if (*p >= '0' && *p <= '9') {
        peak = 0;
        while (*p >= '0' && *p <= '9') peak = peak * 10 + unsigned(*p++ - '0');
        while (*p == ' ') ++p;
        while (*p >= '0' && *p <= '9') avg = avg * 10 + unsigned(*p++ - '0');
    } else if (!off) {
        Out o(out, cap);
        o.add("usage: bus [read] | bus off | bus PEAK_MBPS [AVG_MBPS]  (boot: fajita-ddr=%u)\n", 7216u);
        return o.n;
    }
    size_t n = run(true, off, peak, avg, out, cap);
    if (n + 1 < cap) {out[n++] = '\n'; out[n] = 0;}
    return n;
}
}  // namespace

// For a governor (FajitaGpuProbe.cpp): peak MB/s (Linux's GPU vote is 7216), 0 = the keepalive minimum. Returns 0 when the BCMs read
// back what was sent; the reply text lands in fajita_bus_row.
extern "C" int fajita_bus_set(unsigned peakMBps) {
    if (!gLock) return -19;
    run(true, peakMBps == 0, peakMBps, 0, fajita_bus_row, sizeof fajita_bus_row);
    return int(fajita_bus_state[3]);
}

// For other board code (FajitaWifiProbe.cpp: the modem's cx/mx/mss ARC votes): cmd-db's address of resource `name` (up to 8
// characters, e.g. "mss.lvl") and up to *auxLen bytes of its aux data (an ARC's u16 voltage levels). 0 on success.
extern "C" int fajita_rpmh_lookup(const char *name, uint32_t *addr, uint8_t *aux, unsigned *auxLen) {
    IODeviceMemory *m = IODeviceMemory::withRange(kCmdDbBase, kCmdDbSize);
    if (!m) return -12;
    OSSharedPtr<IOMemoryMap> map(m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
    m->release();
    if (!map) return -12;
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return -12;
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < kCmdDbSize / 4; ++i) {const uint32_t w = src[i]; memcpy(db + 4 * i, &w, 4);}
    enum {kHeader = 8, kRscHdr = 16, kSlaves = 8, kData = 8 + 16 * 8 + 8, kEntry = 24};
    int result = -2;
    char want[8] = {};
    strncpy(want, name, sizeof want);
    if (db[4] == 0xdb && db[5] == 0x30 && db[6] == 0x03 && db[7] == 0x0c) {
        for (unsigned i = 0; i < kSlaves && result; ++i) {
            const uint8_t *h = db + kHeader + kRscHdr * i;
            const unsigned slave = le16(h), header = le16(h + 2), data = le16(h + 4), count = le16(h + 6);
            if (!slave) break;
            for (unsigned j = 0; j < count; ++j) {
                const size_t at = size_t(kData) + header + size_t(j) * kEntry;
                if (at + kEntry > kCmdDbSize) break;
                const uint8_t *e = db + at;
                if (memcmp(e, want, 8)) continue;
                *addr = le32(e + 16);
                const size_t len = le16(e + 20), off = size_t(kData) + data + le16(e + 22);
                unsigned n = 0;
                if (aux && auxLen && off + len <= kCmdDbSize) {n = len < *auxLen ? unsigned(len) : *auxLen; memcpy(aux, db + off, n);}
                if (auxLen) *auxLen = n;
                result = 0;
                break;
            }
        }
    }
    IOFreeData(db, kCmdDbSize);
    return result;
}

// One AMC request of n (<= 16) {msgid, addr, data} commands over the apps RSC, serialised with the bus votes. Returns the TCS
// used, -1 none idle, -2 no completion within 100 ms, -19 before the bus code is up. resp (may be null) gets read responses.
extern "C" int fajita_rpmh_send(unsigned n, const uint32_t (*cmd)[3], uint32_t *resp) {
    if (!gLock || !n || n > 16) return -19;
    IOLockLock(gLock);
    if (!gRsc) {
        IODeviceMemory *m = IODeviceMemory::withRange(kRscBase, 0x10000);
        if (m) {gRsc = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain); m->release();}
    }
    uint32_t st[16] = {}, us = 0;
    const int t = gRsc ? send(n, cmd, st, resp, &us) : -12;
    IOLockUnlock(gLock);
    return t;
}

// Called by FajitaCpuFreq.cpp's sheng_cpufreq_poll (the boot progress monitor, once a second): the first call after IOKit is up
// registers the kcmd and sends the boot vote.
extern "C" void fajita_bus_poll(void) {
    static bool done;
    if (done || !IOService::getPlatform()) return;
    done = true;
    gLock = IOLockAlloc();
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("bus", kcmdBus, "DDR/LLCC BCM votes: bus [read | off | PEAK_MBPS [AVG_MBPS]]");
    unsigned peak = 7216;
    PE_parse_boot_argn("fajita-ddr", &peak, sizeof peak);
    if (!peak) {strlcpy(fajita_bus_row, "bus: fajita-ddr=0, firmware votes kept", sizeof fajita_bus_row); return;}
    run(true, false, peak, 0, fajita_bus_row, sizeof fajita_bus_row);
    IOLog("fajita-bus: %s\n", fajita_bus_row);
}
