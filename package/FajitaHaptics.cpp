// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita vibration motor: the PMI8998 haptics block (SPMI SID 3, base 0xc000) driven through NativeSpmi, with the register
// sequence of the Qualcomm SPMI haptics driver that mainline OnePlus 6/6T work uses (Caleb Connolly, "input: add Qualcomm SPMI
// haptics driver" v7, drivers/input/misc/qcom-pmi8998-haptics.c: haptics_init, haptics_vibrate, haptics_stop_vibrate; DT
// qcom,pmi8998-haptics reg 0xc000, qcom,wave-play-rate-us 4255, sdm845-oneplus-common.dtsi). LRA, buffer mode, sine, 400 mA.
// Only the registers that driver writes are written, with its values; no PON, reset or power-path register is touched.
//
// Boot argument fajita-haptics=<n> (default 0: nothing is read or written):
//   1  registers `kcmd vibrate`  (vibrate status | vibrate MS [VMAX_MV] | vibrate stop)
//   2  as 1 and reads the haptics block read-only at boot into the log
//   3  as 2, and iOS's haptics: /dev/fajita-haptic (mode 0666) takes 16-bit levels (0..1000, 0 = stop) from the HAL plug-in
//      (package/fajita-audio/FajitaAudio.c, the Taptic "Actuator" device iOS plays its feedback and vibrations on) and drives the
//      motor by them (FajitaHapticPolicy.h: start, drive changes, 25 ms minimum, 150 ms keepalive, 5 s cap). A write starting with
//      0xffff is a text line of the plug-in's, logged as "fajita-audio: ...". The kernel also mounts
//      the plug-in over /Library/Audio/Plug-Ins/HAL with this mode (ios16/patches/xnu-8796/zzy-native-gpu-addon-mount.patch).
// The block is configured on the first vibrate (haptics_init), never at boot.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include "NativeSpmi.h"
#include "FajitaHapticPolicy.h"
extern "C" {
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>
}

extern "C" {
volatile uint32_t fajita_haptics_state[4];   // [0] arg, [1] vibrations, [2] last error, [3] last status
// iOS's haptics (mode 3): [0] writes, [1] levels, [2] starts, [3] stops, [4] keepalive timeouts, [5] 5 s caps, [6] SPMI errors,
// [7] the last level
volatile uint32_t fajita_haptics_ios[8];
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
}

namespace {
enum : unsigned {
    kSid = 3, kBase = 0xc000,
    kStatus1 = 0x0a, kBusy = 1u << 1, kScFlag = 1u << 3, kAutoResErr = 1u << 4,
    kEnCtl = 0x46, kEnBit = 1u << 7,
    kEnCtl2 = 0x48, kBrakeEn = 1u << 0,
    kCfg1 = 0x4c, kCfg2 = 0x4d, kSel = 0x4e, kAutoRes = 0x4f, kVmaxCfg = 0x51, kIlim = 0x52, kScDeb = 0x53, kRate1 = 0x54, kRate2 = 0x55,
    kScClr = 0x59, kBrake = 0x5c, kWfRepeat = 0x5e, kWfS1 = 0x60, kPlay = 0x70, kPlayBit = 1u << 7, kPauseBit = 1u << 0,
    kVmaxMin = 116, kVmaxMax = 3596, kMaxMs = 5000,
};
const uint8_t kWave[8] = {0x7e, 0x7e, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28};
bool gInit;
IOLock *gLock;
fhp_motor gMotor;   // iOS's haptics (mode 3), under gLock
bool gIosOn;

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

int rd(unsigned reg) {return native_spmi_read8(kSid, kBase + reg);}
int mw(unsigned reg, unsigned mask, unsigned val) {return native_spmi_masked_write(kSid, kBase + reg, mask, val);}

// haptics_init for LRA / buffer / sine / 400 mA / wave-play-rate 4255 us.
int init() {
    int r;
    if ((r = native_spmi_init())) return r;
    unsigned rate = 4255 / 5;
    if ((r = mw(kCfg1, 0x01, 0))) return r;                                    // HAP_TYPE_LRA
    if ((r = mw(kAutoRes, 0x7f, (3u) | (4u << 4) | (1u << 2)))) return r;      // res cal 32/4 periods, ZXD_EOP, high-Z 1
    if ((r = mw(kSel, 0x30, 1u << 4))) return r;                               // buffer mode
    if ((r = mw(kIlim, 0x01, 0))) return r;                                    // 400 mA
    if ((r = mw(kScDeb, 0x07, 32 & 7))) return r;                              // debounce (HAP_SC_DEB_CYCLES_MAX masked)
    if ((r = mw(kCfg2, 0x01, 0))) return r;                                    // sine
    if ((r = mw(kRate1, 0xff, rate & 0xff))) return r;
    if ((r = mw(kRate2, 0xff, (rate >> 8) & 0x0f))) return r;
    if ((r = mw(kBrake, 0xff, 0xff))) return r;                                // brake pattern 3,3,3,3
    if ((r = mw(kEnCtl2, kBrakeEn, kBrakeEn))) return r;
    if ((r = mw(kWfRepeat, 0x73, 0))) return r;
    if ((r = native_spmi_write(kSid, kBase + kWfS1, kWave, sizeof kWave))) return r;
    gInit = true;
    return 0;
}

int stop() {
    int r = mw(kPlay, kPlayBit | kPauseBit, 0);
    int r2 = mw(kEnCtl, kEnBit, 0);
    return r ? r : r2;
}

int setVmax(unsigned vmaxMv) {
    if (vmaxMv < kVmaxMin) vmaxMv = kVmaxMin;
    if (vmaxMv > kVmaxMax) vmaxMv = kVmaxMax;
    unsigned step = (vmaxMv + kVmaxMin / 2) / kVmaxMin;                         // DIV_ROUND_CLOSEST
    return mw(kVmaxCfg, 0x7e | 0x40, (step & 0x1f) << 1);
}

// Start playing at vmaxMv and return at once (iOS's haptics; vibrate() below is the blocking kcmd form).
int start(unsigned vmaxMv) {
    int r;
    if (!gInit && (r = init())) return r;
    if ((r = setVmax(vmaxMv))) return r;
    if ((r = mw(kEnCtl, kEnBit, kEnBit))) return r;
    if ((r = mw(kPlay, kPlayBit | kPauseBit, kPlayBit))) {stop(); return r;}
    return 0;
}

int vibrate(unsigned ms, unsigned vmaxMv, Out &o) {
    int r;
    if (!gInit && (r = init())) {o.add("init %d", r); return r;}
    if (vmaxMv < kVmaxMin) vmaxMv = kVmaxMin;
    if (vmaxMv > kVmaxMax) vmaxMv = kVmaxMax;
    unsigned step = (vmaxMv + kVmaxMin / 2) / kVmaxMin;                         // DIV_ROUND_CLOSEST
    if ((r = setVmax(vmaxMv))) {o.add("vmax %d", r); return r;}
    if ((r = mw(kEnCtl, kEnBit, kEnBit))) {o.add("enable %d", r); return r;}
    if ((r = mw(kPlay, kPlayBit | kPauseBit, kPlayBit))) {o.add("play %d", r); stop(); return r;}
    o.add("vibrating %u ms at %u mV (step %u) ", ms, step * kVmaxMin, step);
    unsigned busy = 0, sc = 0, scTotal = 0;
    for (unsigned t = 0; t < ms; t += 20) {
        IOSleep(20);
        int s = rd(kStatus1);
        if (s >= 0 && (s & kBusy)) ++busy;
        // The mainline driver clears SC_FLAG (HAP_SC_CLR_REG bit 0) each time it is seen and gives up after 5 in a row.
        if (s >= 0 && (s & kScFlag)) {++scTotal; mw(kScClr, 1, 1); if (++sc > 5) {o.add("SHORT-CIRCUIT persists, stopping "); break;}}
        else sc = 0;
    }
    if (scTotal) o.add("(sc flag seen %u times) ", scTotal);
    r = stop();
    o.add("busy in %u polls, stop %d", busy, r);
    return r;
}

size_t kcmdVibrate(const char *args, size_t len, char *out, size_t cap) {
    char a[40] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    Out o(out, cap);
    IOLockLock(gLock);
    native_spmi_init();
    if (!strncmp(a, "stop", 4)) {int r = stop(); o.add("stop %d", r);}
    else if (a[0] >= '0' && a[0] <= '9') {
        unsigned ms = 0, mv = 3000;
        const char *p = a;
        while (*p >= '0' && *p <= '9') ms = ms * 10 + unsigned(*p++ - '0');
        while (*p == ' ') ++p;
        if (*p >= '0' && *p <= '9') {mv = 0; while (*p >= '0' && *p <= '9') mv = mv * 10 + unsigned(*p++ - '0');}
        if (ms > kMaxMs) ms = kMaxMs;
        int r = vibrate(ms, mv, o);
        fajita_haptics_state[1] = fajita_haptics_state[1] + 1;
        fajita_haptics_state[2] = uint32_t(r);
    } else {
        int s1 = rd(kStatus1), en = rd(kEnCtl), pl = rd(kPlay), sel = rd(kSel), r1 = rd(kRate1), r2 = rd(kRate2), vm = rd(kVmaxCfg), a1 = rd(0x0b), a2 = rd(0x0c);
        o.add("haptics sid%u@%x: STATUS1 %x EN %x PLAY %x SEL %x RATE %x/%x VMAX %x AUTORES_RD %x/%x init %d vibrations %u (usage: vibrate MS [VMAX_MV] | stop)",
              kSid, kBase, s1, en, pl, sel, r1, r2, vm, a1, a2, gInit, (unsigned)fajita_haptics_state[1]);
        if (gIosOn)
            o.add("\niOS: writes %u levels %u starts %u stops %u timeouts %u caps %u spmi-errors %u last %u playing %d",
                  (unsigned)fajita_haptics_ios[0], (unsigned)fajita_haptics_ios[1], (unsigned)fajita_haptics_ios[2], (unsigned)fajita_haptics_ios[3],
                  (unsigned)fajita_haptics_ios[4], (unsigned)fajita_haptics_ios[5], (unsigned)fajita_haptics_ios[6], (unsigned)fajita_haptics_ios[7],
                  gMotor.playing);
    }
    IOLockUnlock(gLock);
    o.add("\n");
    return o.n;
}
// ---- iOS's haptics (mode 3) ----

uint64_t nowMs() {
    uint64_t t, ns;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000000;
}

// With gLock held.
void act(int a, unsigned vmax) {
    int r = 0;
    if (a == FHP_START) {r = start(vmax); fajita_haptics_ios[2] = gMotor.starts; IOLockWakeup(gLock, &gMotor, false);}
    else if (a == FHP_UPDATE) r = setVmax(vmax);
    else if (a == FHP_STOP) {r = stop(); fajita_haptics_ios[3] = gMotor.stops;}
    if (r) {
        fajita_haptics_ios[6] = fajita_haptics_ios[6] + 1;
        if (fajita_haptics_ios[6] == 1) IOLog("fajita-haptics: SPMI error %d driving iOS's haptics\n", r);
    }
}

void iosThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-haptics");
    IOLockLock(gLock);
    for (;;) {
        if (!gMotor.playing) {IOLockSleep(gLock, &gMotor, THREAD_UNINT); continue;}
        act(fhp_tick(&gMotor, nowMs()), 0);
        fajita_haptics_ios[4] = gMotor.timeouts;
        fajita_haptics_ios[5] = gMotor.capped;
        IOLockUnlock(gLock);
        IOSleep(5);
        IOLockLock(gLock);
    }
}

int hapticOpen(dev_t, int, int, struct proc *) {return 0;}
int hapticClose(dev_t, int, int, struct proc *) {return 0;}
int hapticWrite(dev_t, struct uio *uio, int) {
    uint16_t levels[32];
    // A write that starts with 0xffff is a diagnostic line of the plug-in (no level is above 1000): into the kernel log, counted.
    if (uio_resid(uio) > 2 && uio_resid(uio) <= 200) {
        char line[202] = {};
        const int n = int(uio_resid(uio));
        int err = uiomove(line, n, uio);
        if (err) return err;
        if ((uint8_t)line[0] == 0xff && (uint8_t)line[1] == 0xff) {
            static unsigned said;
            if (said++ < 200) IOLog("fajita-audio: %s\n", line + 2);
            return 0;
        }
        // not a line: levels after all
        IOLockLock(gLock);
        native_spmi_init();
        fajita_haptics_ios[0] = fajita_haptics_ios[0] + 1;
        for (int i = 0; i + 1 < n; i += 2) {
            uint16_t l16; memcpy(&l16, line + i, 2);
            unsigned level = l16 > 1000 ? 1000 : l16, vmax = 0;
            fajita_haptics_ios[1] = fajita_haptics_ios[1] + 1;
            fajita_haptics_ios[7] = level;
            act(fhp_command(&gMotor, nowMs(), level, &vmax), vmax);
        }
        IOLockUnlock(gLock);
        return 0;
    }
    while (uio_resid(uio) > 0) {
        int n = int(uio_resid(uio) < int(sizeof levels) ? uio_resid(uio) : sizeof levels);
        int err = uiomove(reinterpret_cast<char *>(levels), n, uio);
        if (err) return err;
        fajita_haptics_ios[0] = fajita_haptics_ios[0] + 1;
        IOLockLock(gLock);
        native_spmi_init();
        for (int i = 0; i < n / 2; ++i) {
            unsigned level = levels[i] > 1000 ? 1000 : levels[i], vmax = 0;
            fajita_haptics_ios[1] = fajita_haptics_ios[1] + 1;
            fajita_haptics_ios[7] = level;
            act(fhp_command(&gMotor, nowMs(), level, &vmax), vmax);
        }
        IOLockUnlock(gLock);
    }
    return 0;
}
struct cdevsw gHapticSw = {
    hapticOpen, hapticClose, eno_rdwrt, hapticWrite, eno_ioctl, eno_stop, eno_reset, nullptr,
    eno_select, eno_mmap, eno_strat, eno_getc, eno_putc, 0,
};

void iosStart() {
    thread_t t = nullptr;
    if (kernel_thread_start(iosThread, nullptr, &t) != KERN_SUCCESS) {IOLog("fajita-haptics: no thread for iOS's haptics\n"); return;}
    thread_deallocate(t);
    int major = cdevsw_add(-1, &gHapticSw);
    if (major < 0) {IOLog("fajita-haptics: cdevsw_add failed\n"); return;}
    // 0666: mediaserverd, which loads the HAL plug-in, runs as mobile.
    if (!devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "fajita-haptic")) {IOLog("fajita-haptics: no /dev node\n"); return;}
    gIosOn = true;
    IOLog("fajita-haptics: /dev/fajita-haptic ready for iOS's haptics\n");
}
}  // namespace

extern "C" int fajita_haptics_pulse(unsigned ms) {   // for the iOS vibration hook
    if (!gLock) return -19;
    char buf[160];
    Out o(buf, sizeof buf);
    IOLockLock(gLock);
    int r = vibrate(ms > kMaxMs ? kMaxMs : ms, 3000, o);
    IOLockUnlock(gLock);
    return r;
}

extern "C" void fajita_haptics_poll(void) {
    static bool done;
    if (done || !IOService::getPlatform()) return;
    done = true;
    unsigned mode = 0;
    PE_parse_boot_argn("fajita-haptics", &mode, sizeof mode);
    fajita_haptics_state[0] = mode;
    if (!mode) return;
    gLock = IOLockAlloc();
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("vibrate", kcmdVibrate, "haptics: vibrate MS [VMAX_MV] | stop | (status)");
    if (mode >= 2) {
        native_spmi_init();
        IOLog("fajita-haptics: sid%u@%x STATUS1 %x EN %x PLAY %x SEL %x RATE %x/%x VMAX %x\n", kSid, kBase, rd(kStatus1), rd(kEnCtl), rd(kPlay), rd(kSel),
              rd(kRate1), rd(kRate2), rd(kVmaxCfg));
    }
    if (mode >= 3) iosStart();
}
