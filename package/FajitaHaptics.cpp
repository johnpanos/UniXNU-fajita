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
// The block is configured on the first vibrate (haptics_init), never at boot.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "NativeSpmi.h"

extern "C" {
volatile uint32_t fajita_haptics_state[4];   // [0] arg, [1] vibrations, [2] last error, [3] last status
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

int vibrate(unsigned ms, unsigned vmaxMv, Out &o) {
    int r;
    if (!gInit && (r = init())) {o.add("init %d", r); return r;}
    if (vmaxMv < kVmaxMin) vmaxMv = kVmaxMin;
    if (vmaxMv > kVmaxMax) vmaxMv = kVmaxMax;
    unsigned step = (vmaxMv + kVmaxMin / 2) / kVmaxMin;                         // DIV_ROUND_CLOSEST
    if ((r = mw(kVmaxCfg, 0x7e | 0x40, (step & 0x1f) << 1))) {o.add("vmax %d", r); return r;}
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
    }
    IOLockUnlock(gLock);
    o.add("\n");
    return o.n;
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
}
